#!/usr/bin/env python3
"""Clotho performance benchmark harness.

Subcommands:
  run       benchmark clotho on all (or selected) cases -> results.json
  compare   gate results.json against baseline.json (exit 1 on regression)
  baseline  benchmark and write baseline.json (the manual sign-off step)
  oracles   benchmark iverilog/verilator -> oracles.json (manual, never CI)

Methodology: hyperfine wall clock (warm cache, minimum of N runs, default 5;
minimum is the estimator because run-time noise is one-sided); clotho
compile/sim split parsed from --time stderr appended across the same runs.
See README.md for the regression policy.
"""

import argparse
import datetime
import json
import os
import platform
import re
import shlex
import shutil
import statistics
import subprocess
import sys
import tomllib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BUILD_DIR = REPO / "build-perf"
RESULTS_PATH = HERE / "results.json"
BASELINE_PATH = HERE / "baseline.json"
ORACLES_PATH = HERE / "oracles.json"
# Machine-local map {tool: bindir} prepended to PATH for that tool's commands.
# Gitignored: an oracle installed outside the system PATH is machine-specific.
TOOLS_LOCAL_PATH = HERE / "tools.local.json"
DEFAULT_RUNS = 5
SANITY_TIMEOUT_S = 900

# Anchored to the label column of clotho's `--time` block so a stray "compile"
# elsewhere on stderr cannot be scraped as a timing.
PHASE_RES = {
    "frontend_ms": re.compile(r"^  read source\s+([0-9.]+)", re.M),
    "analysis_ms": re.compile(r"^  analyze\s+([0-9.]+)", re.M),
    "llvm_ms": re.compile(r"^  compile\s+([0-9.]+)", re.M),
    "sim_ms": re.compile(r"^  simulate\s+([0-9.]+)", re.M),
}

# Gated metrics: any statistically significant regression fails compare.
# Sim regressions are bugs; compile/wall regressions require manual sign-off.
GATES = [
    ("sim_ms", "bug — fix before merge (or sign off: perf.py baseline)"),
    ("compile_ms", "needs manual sign-off (perf.py baseline)"),
    ("wall_ms", "needs manual sign-off (perf.py baseline)"),
]
# Fixed noise floor for significance; widened by measured run-to-run spread.
MARGIN_FLOOR = 0.02

ORACLE_BINS = {
    "iverilog": ["iverilog", "vvp"],
    "verilator": ["verilator"],
}
ORACLE_VERSION_CMD = {
    "iverilog": ["iverilog", "-V"],
    "verilator": ["verilator", "--version"],
}


def fail(msg):
    print(f"perf.py: {msg}", file=sys.stderr)
    sys.exit(2)


def load_cases(names):
    cases = []
    for case_dir in sorted((HERE / "cases").iterdir()):
        toml_path = case_dir / "case.toml"
        if not toml_path.is_file():
            continue
        with open(toml_path, "rb") as f:
            cfg = tomllib.load(f)
        cfg["_dir"] = case_dir
        cases.append(cfg)
    if names:
        by_name = {c["name"]: c for c in cases}
        missing = [n for n in names if n not in by_name]
        if missing:
            fail(f"unknown case(s): {', '.join(missing)}")
        cases = [by_name[n] for n in names]
    if not cases:
        fail("no cases found under cases/")
    return cases


def ensure_sources(case):
    fetch = case.get("fetch")
    if not fetch:
        return
    if (case["_dir"] / fetch["probe"]).exists():
        return
    print(f"[{case['name']}] fetching sources ({fetch['script']})")
    subprocess.run(["sh", fetch["script"]], cwd=case["_dir"], check=True)


def ensure_build(args):
    if args.clotho:
        # A prebuilt binary (bench-diff.py, cloud/bench-remote.sh); never rebuilt.
        clotho = Path(args.clotho).resolve()
        if not clotho.is_file() or not os.access(clotho, os.X_OK):
            fail(f"--clotho {clotho} is not an executable file")
        return clotho
    clotho = BUILD_DIR / "clotho"
    if args.no_build:
        if not clotho.is_file():
            fail(f"--no-build but {clotho} does not exist")
        return clotho
    cache = BUILD_DIR / "CMakeCache.txt"
    if not cache.is_file() or "CMAKE_BUILD_TYPE:STRING=Release" not in cache.read_text():
        print(f"configuring {BUILD_DIR} (Release)")
        subprocess.run(
            ["cmake", "-S", str(REPO), "-B", str(BUILD_DIR), "-G", "Ninja",
             "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"],
            check=True)
    subprocess.run(["ninja", "-C", str(BUILD_DIR), "clotho"], check=True)
    return clotho


def hyperfine(cmd, cwd, runs, warmup, env=None):
    """Run one shell command under hyperfine; return per-run wall times in ms."""
    export = cwd / "hyperfine.json"
    subprocess.run(
        ["hyperfine", "--style", "basic", "--warmup", str(warmup),
         "--runs", str(runs), "--export-json", str(export), cmd],
        cwd=cwd, check=True, env=env)
    with open(export) as f:
        times_s = json.load(f)["results"][0]["times"]
    return [t * 1000.0 for t in times_s]


def metric(samples):
    return {"samples": [round(s, 3) for s in samples], "min": round(min(samples), 3)}


def sanity_run(cmd, cwd, regex, label, env=None):
    """One untimed correctness run; refuse to benchmark a broken sim."""
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True,
                       timeout=SANITY_TIMEOUT_S, shell=isinstance(cmd, str), env=env)
    if r.returncode != 0:
        fail(f"[{label}] sanity run failed (rc={r.returncode}):\n{r.stdout}\n{r.stderr}")
    if regex and not re.search(regex, r.stdout):
        fail(f"[{label}] sanity run stdout does not match /{regex}/:\n{r.stdout}")


def measure_clotho(case, clotho, runs):
    name = case["name"]
    cwd = case["_dir"]
    files = case["clotho"]["files"]
    args = case["clotho"].get("args", [])
    regex = case.get("check", {}).get("stdout_regex")

    sanity_run([str(clotho), "run", *args, *files], cwd, regex, f"{name}/clotho")

    # One hyperfine invocation yields both the warm wall clock and, via the
    # appended --time stderr, per-run compile/sim splits.
    splits = cwd / "splits.txt"
    splits.unlink(missing_ok=True)
    q = shlex.quote
    cmd = f"{q(str(clotho))} run --time {' '.join(map(q, args + files))} 2>>splits.txt"
    print(f"[{name}] clotho wall + split ({runs} runs, 1 warmup)")
    wall = hyperfine(cmd, cwd, runs, warmup=1)

    text = splits.read_text()
    metrics = {"wall_ms": metric(wall)}
    phase_samples = {}
    for key, rx in PHASE_RES.items():
        vals = [float(v) for v in rx.findall(text)]
        if len(vals) != runs + 1:  # warmup contributes one entry
            fail(f"[{name}] expected {runs + 1} '{key}' entries in splits.txt, got {len(vals)}")
        phase_samples[key] = vals[-runs:]  # drop the warmup
        metrics[key] = metric(phase_samples[key])
    compile_samples = [
        f + a + l for f, a, l in zip(phase_samples["frontend_ms"],
                                     phase_samples["analysis_ms"],
                                     phase_samples["llvm_ms"])
    ]
    metrics["compile_ms"] = metric(compile_samples)
    return metrics


def run_meta(runs):
    commit = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=REPO,
                            capture_output=True, text=True).stdout.strip()
    cpu = ""
    try:
        for line in open("/proc/cpuinfo"):
            if line.startswith("model name"):
                cpu = line.split(":", 1)[1].strip()
                break
    except OSError:
        pass
    return {"date": datetime.date.today().isoformat(), "commit": commit,
            "host": platform.node(), "cpu": cpu, "runs": runs}


def cmd_run(args, out_path):
    cases = load_cases(args.cases)
    clotho = ensure_build(args)
    doc = {"schema": 1, "cases": {}}
    if out_path.is_file():  # merge: a partial run keeps other cases' numbers
        with open(out_path) as f:
            doc = json.load(f)
    for case in cases:
        ensure_sources(case)
        metrics = measure_clotho(case, clotho, args.runs)
        doc["cases"][case["name"]] = {"meta": run_meta(args.runs), "metrics": metrics}
    with open(out_path, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"wrote {out_path}")
    for name, entry in doc["cases"].items():
        m = entry["metrics"]
        print(f"  {name}: sim {m['sim_ms']['min']} ms | compile {m['compile_ms']['min']} ms"
              f" | wall {m['wall_ms']['min']} ms")


def rel_spread(m):
    """Noise of the min estimator: how far the median sits above the min.

    Robust to one-sided outliers (a single slow run inflates stddev/CV by an
    order of magnitude but leaves median - min untouched).
    """
    return statistics.median(m["samples"]) / m["min"] - 1.0 if m["samples"] else 0.0


def cmd_compare(args):
    for p in (args.results, args.baseline):
        if not Path(p).is_file():
            fail(f"missing {p}")
    with open(args.results) as f:
        results = json.load(f)
    with open(args.baseline) as f:
        baseline = json.load(f)

    regressions = []
    for name, entry in results["cases"].items():
        base_entry = baseline["cases"].get(name)
        if base_entry is None:
            print(f"{name}: no baseline (new case?) — record one with perf.py baseline")
            continue
        print(f"{name} (baseline {base_entry['meta']['commit']} {base_entry['meta']['date']}):")
        for key, policy in GATES:
            new_m, base_m = entry["metrics"][key], base_entry["metrics"][key]
            delta = new_m["min"] / base_m["min"] - 1.0
            # Significance margin: fixed floor widened by the larger measured
            # run-to-run spread of either side.
            margin = max(MARGIN_FLOOR, 2.0 * max(rel_spread(base_m), rel_spread(new_m)))
            if delta > margin:
                verdict = f"REGRESSION ({policy})"
                regressions.append((name, key))
            elif delta < -margin:
                verdict = "improved"
            else:
                verdict = "ok"
            print(f"  {key:<12} {base_m['min']:>10.3f} -> {new_m['min']:>10.3f} ms"
                  f"  {delta:+7.2%} (margin {margin:.2%})  {verdict}")
        info = "  ".join(
            f"{k} {base_entry['metrics'][k]['min']:.1f}->{entry['metrics'][k]['min']:.1f}"
            for k in ("frontend_ms", "analysis_ms", "llvm_ms"))
        print(f"  phases (info): {info}")

    if ORACLES_PATH.is_file():
        with open(ORACLES_PATH) as f:
            oracles = json.load(f)
        for name, tools in oracles["cases"].items():
            if name not in results["cases"]:
                continue
            ctx = ", ".join(
                f"{t}: compile {v['compile_ms']['min']:.0f} / sim {v['sim_ms']['min']:.0f} ms"
                for t, v in tools.items() if "sim_ms" in v)
            if ctx:
                print(f"{name} oracle context (static, non-gating): {ctx}")

    if regressions:
        print(f"\nFAIL: {len(regressions)} regression(s): "
              + ", ".join(f"{n}/{k}" for n, k in regressions))
        sys.exit(1)
    print("\nOK: no significant regressions")


def oracle_cmds(tool, files, top, incdirs=()):
    """Return (compile_cmd, sim_cmd) shell strings, run from the case dir.

    incdirs lists `include search directories (case-relative). Clotho's slang
    frontend searches the including file's own directory, so a case that stages
    a flat source tree needs no incdir for clotho — but iverilog and verilator
    do, hence this per-tool flag threading. Empty incdirs leaves commands byte
    for byte as before.
    """
    fq = " ".join(shlex.quote(f) for f in files)
    dirs = [shlex.quote(d) for d in incdirs]
    inc_dash_i   = "".join(f" -I {d}" for d in dirs)          # iverilog
    inc_verilator = "".join(f" -I{d}" for d in dirs)          # verilator
    if tool == "iverilog":
        return f"iverilog -g2012{inc_dash_i} -o iv.out {fq}", "vvp iv.out"
    if tool == "verilator":
        return (f"rm -rf obj_dir && verilator --binary --timing -Wno-fatal -j 0"
                f"{inc_verilator} --top-module {top} {fq}",
                f"obj_dir/V{top}")
    fail(f"unknown oracle tool '{tool}'")


def tool_version(tool, env):
    r = subprocess.run(ORACLE_VERSION_CMD[tool], capture_output=True, text=True, env=env)
    lines = (r.stdout or r.stderr).strip().splitlines()
    return lines[0].strip() if lines else "unknown"


def load_tools_local():
    if not TOOLS_LOCAL_PATH.is_file():
        return {}
    with open(TOOLS_LOCAL_PATH) as f:
        return json.load(f)


def tool_env(tool, tools_local):
    """Environment for a tool's commands, with its local bindir prepended to PATH."""
    bindir = tools_local.get(tool)
    if not bindir:
        return None
    env = os.environ.copy()
    env["PATH"] = f"{bindir}{os.pathsep}{env.get('PATH', '')}"
    return env


def on_path(bins, env):
    path = env["PATH"] if env else None
    return all(shutil.which(b, path=path) is not None for b in bins)


def cmd_oracles(args):
    cases = load_cases(args.cases)
    tools_local = load_tools_local()
    doc = {"schema": 1, "cases": {}}
    if ORACLES_PATH.is_file():  # merge per case/tool across manual runs
        with open(ORACLES_PATH) as f:
            doc = json.load(f)
    for case in cases:
        ensure_sources(case)
        name = case["name"]
        cwd = case["_dir"]
        top = case.get("top")
        regex = case.get("check", {}).get("stdout_regex")
        entry = doc["cases"].setdefault(name, {})
        for tool, tool_cfg in case.get("oracles", {}).items():
            if args.tools and tool not in args.tools:
                continue
            if not tool_cfg.get("enabled", True):
                print(f"[{name}/{tool}] disabled: {tool_cfg.get('note', 'no note')}")
                entry[tool] = {"skipped": tool_cfg.get("note", "disabled in case.toml")}
                continue
            env = tool_env(tool, tools_local)
            if not on_path(ORACLE_BINS[tool], env):
                hint = "" if tool in tools_local else f" (set its bindir in {TOOLS_LOCAL_PATH.name})"
                print(f"[{name}/{tool}] SKIPPED: not on PATH{hint} (existing numbers kept)")
                continue
            files = tool_cfg.get("files", case["clotho"]["files"])
            incdirs = case.get("incdirs", [])
            compile_cmd, sim_cmd = oracle_cmds(tool, files, top, incdirs)
            print(f"[{name}/{tool}] sanity compile + sim")
            sanity_run(compile_cmd, cwd, None, f"{name}/{tool}/compile", env=env)
            sanity_run(sim_cmd, cwd, regex, f"{name}/{tool}/sim", env=env)
            print(f"[{name}/{tool}] timing compile ({args.runs} runs)")
            compile_times = hyperfine(compile_cmd, cwd, args.runs, warmup=0, env=env)
            print(f"[{name}/{tool}] timing sim ({args.runs} runs)")
            sim_times = hyperfine(sim_cmd, cwd, args.runs, warmup=0, env=env)
            entry[tool] = {
                "meta": {**run_meta(args.runs), "version": tool_version(tool, env)},
                "compile_ms": metric(compile_times),
                "sim_ms": metric(sim_times),
            }
    with open(ORACLES_PATH, "w") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    print(f"wrote {ORACLES_PATH}")


def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    def add_common(sp):
        sp.add_argument("--runs", type=int, default=DEFAULT_RUNS)
        sp.add_argument("--cases", nargs="+", metavar="NAME")

    def add_binary(sp):
        g = sp.add_mutually_exclusive_group()
        g.add_argument("--no-build", action="store_true",
                       help=f"use the existing {BUILD_DIR.name}/clotho without rebuilding")
        g.add_argument("--clotho", metavar="BIN",
                       help=f"benchmark this binary instead of building {BUILD_DIR.name}/clotho")

    sp = sub.add_parser("run", help="benchmark clotho -> results.json")
    add_common(sp)
    add_binary(sp)
    sp.add_argument("--out", metavar="JSON", default=str(RESULTS_PATH),
                    help="results file; existing entries for other cases are kept"
                         " (default %(default)s)")
    sp = sub.add_parser("baseline", help="benchmark clotho -> baseline.json (sign-off)")
    add_common(sp)
    add_binary(sp)
    sp = sub.add_parser("compare", help="gate results.json against baseline.json")
    sp.add_argument("--results", default=str(RESULTS_PATH))
    sp.add_argument("--baseline", default=str(BASELINE_PATH))
    sp = sub.add_parser("oracles", help="benchmark iverilog/verilator -> oracles.json")
    add_common(sp)
    sp.add_argument("--tools", nargs="+", choices=sorted(ORACLE_BINS), metavar="TOOL")

    args = p.parse_args()
    if args.cmd == "run":
        cmd_run(args, Path(args.out))
    elif args.cmd == "baseline":
        cmd_run(args, BASELINE_PATH)
    elif args.cmd == "compare":
        cmd_compare(args)
    elif args.cmd == "oracles":
        cmd_oracles(args)


if __name__ == "__main__":
    main()
