#!/usr/bin/env python3
"""Differential clotho benchmark across two git refs.

Builds clotho at --base and --head in throwaway worktrees, benchmarks each
against the SAME workload (the cases/ tree of the invoking checkout, not the
ref's), then runs `perf.py compare` so the existing significance-gated
regression policy decides pass/fail. Exit code is compare's: 0 clean, 1 on a
significant regression.

Only the clotho binary varies between the two runs; the SV sources, cycle
counts, hyperfine settings, and host are identical, which is what makes the
delta attributable to the change under test.

    bench-diff.py --base main --head my-feature
    bench-diff.py --base HEAD~1 --head HEAD --cases picorv32 gate_rca --runs 5

Meant to run on a quiet, dedicated box (see the cloud harness). On a busy
machine the numbers are noise, but the mechanics are identical.
"""

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
PERF = HERE / "perf.py"


def repo_root():
    r = subprocess.run(["git", "rev-parse", "--show-toplevel"], cwd=HERE,
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("bench-diff: not inside a git repo")
    return Path(r.stdout.strip())


def resolve_ref(repo, ref):
    r = subprocess.run(["git", "rev-parse", "--verify", "--quiet", f"{ref}^{{commit}}"],
                       cwd=repo, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"bench-diff: unknown ref '{ref}'")
    return r.stdout.strip()


def build_clotho(repo, ref, work):
    """Create a detached worktree at ref, build Release clotho, return (worktree, binary)."""
    wt = work / f"wt-{ref[:12]}"
    if wt.exists():
        subprocess.run(["git", "worktree", "remove", "--force", str(wt)], cwd=repo)
    print(f"[bench-diff] worktree {ref[:12]} -> {wt}")
    subprocess.run(["git", "worktree", "add", "--detach", str(wt), ref], cwd=repo, check=True)
    build = wt / "build-diff"
    # Match perf.py's flags; CXX_SCAN_FOR_MODULES=OFF avoids the dyndep abort on
    # reconfigure.
    subprocess.run(
        ["cmake", "-S", str(wt), "-B", str(build), "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_SCAN_FOR_MODULES=OFF"],
        check=True)
    subprocess.run(["ninja", "-C", str(build), "clotho"], check=True)
    binary = build / "clotho"
    if not binary.is_file():
        sys.exit(f"bench-diff: build produced no clotho at {binary}")
    return wt, binary


def run_perf(binary, out, cases, runs):
    cmd = [sys.executable, str(PERF), "run", "--clotho", str(binary),
           "--out", str(out), "--runs", str(runs)]
    if cases:
        cmd += ["--cases", *cases]
    subprocess.run(cmd, check=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", required=True, help="baseline ref (e.g. main)")
    ap.add_argument("--head", required=True, help="ref with the change under test")
    ap.add_argument("--cases", nargs="+", metavar="NAME", help="subset of cases (default all)")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--work-dir", metavar="DIR",
                    help="scratch dir for worktrees + result json (default <repo>/.bench-diff)")
    ap.add_argument("--keep", action="store_true", help="keep worktrees for debugging")
    args = ap.parse_args()

    repo = repo_root()
    base = resolve_ref(repo, args.base)
    head = resolve_ref(repo, args.head)
    if base == head:
        sys.exit(f"bench-diff: --base and --head resolve to the same commit {base[:12]}")

    work = Path(args.work_dir) if args.work_dir else repo / ".bench-diff"
    work.mkdir(parents=True, exist_ok=True)

    outputs = {}
    for label, ref in (("base", base), ("head", head)):
        wt, binary = build_clotho(repo, ref, work)
        out = work / f"{label}.json"
        try:
            run_perf(binary, out, args.cases, args.runs)
        finally:
            if not args.keep:
                subprocess.run(["git", "worktree", "remove", "--force", str(wt)], cwd=repo)
        outputs[label] = out

    print(f"\n[bench-diff] base {base[:12]} vs head {head[:12]}")
    cmp = subprocess.run(
        [sys.executable, str(PERF), "compare",
         "--baseline", str(outputs["base"]), "--results", str(outputs["head"])])
    sys.exit(cmp.returncode)


if __name__ == "__main__":
    main()
