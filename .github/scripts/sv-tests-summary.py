#!/usr/bin/env python3
"""Classify an sv-tests report for clotho and fail on any crash.

Usage: sv-tests-summary.py REPORT_CSV

Prints a Markdown summary. Outcome classes, from the report's Pass and
ExitCode columns (tools/BaseRunner.py in sv-tests):

  pass      the test's expectation held
  crash     clotho died by a signal (negative exit code)
  timeout   the harness killed the run (exit code 71)
  ran       clotho exited 0 but the test failed: a wrong result, or an
            invalid design accepted -- where silent miscompiles show up
  rejected  clotho refused the design with a diagnostic

Exits 1 if any test crashed (docs/VERIFICATION.md: never crash).
"""

import csv
import sys
from collections import Counter

TIMEOUT_EXIT = 71
LISTED = 50


def outcome(row):
    if row["Pass"] == "True":
        return "pass"
    code = int(row["ExitCode"])
    if code < 0:
        return "crash"
    if code == TIMEOUT_EXIT:
        return "timeout"
    if code == 0:
        return "ran"
    return "rejected"


def main():
    with open(sys.argv[1], newline="") as f:
        rows = [r for r in csv.DictReader(f) if r["Tool"] == "clotho"]
    by_class = {}
    for row in rows:
        by_class.setdefault(outcome(row), []).append(row)
    counts = Counter({k: len(v) for k, v in by_class.items()})

    print(f"## sv-tests: {counts['pass']}/{len(rows)} pass\n")
    print("| outcome | tests |\n|---|---|")
    for name in ("pass", "crash", "timeout", "ran", "rejected"):
        print(f"| {name} | {counts[name]} |")
    for name in ("crash", "timeout", "ran"):
        listed = sorted(by_class.get(name, []), key=lambda r: r["TestName"])
        if not listed:
            continue
        print(f"\n### {name}\n")
        for row in listed[:LISTED]:
            print(f"- `{row['TestName']}` (exit {row['ExitCode']})")
        if len(listed) > LISTED:
            print(f"- ... {len(listed) - LISTED} more")
    return 1 if counts["crash"] else 0


if __name__ == "__main__":
    sys.exit(main())
