#!/usr/bin/env python3
"""Classify an sv-tests report for clotho and fail on any crash.

Usage: sv-tests-summary.py REPORT_CSV LOGS_DIR

Prints a Markdown summary. Outcome classes, from the report's Pass and
ExitCode columns (tools/BaseRunner.py in sv-tests) and each test's log:

  pass        the test's expectation held
  self-check  sv-tests counts a pass, but the design printed a FAILED line:
              imported suites such as ivtest check their own results and
              report them in their output, which sv-tests does not read
  crash       clotho died by a signal (negative exit code)
  timeout     the harness killed the run (exit code 71)
  ran         clotho exited 0 but the test failed: a wrong result, or an
              invalid design accepted
  rejected    clotho refused the design with a diagnostic

self-check and ran are where silent miscompiles show up. Exits 1 if any test
crashed (docs/VERIFICATION.md: never crash).
"""

import csv
import re
import sys
from collections import Counter
from pathlib import Path

TIMEOUT_EXIT = 71
LISTED = 50
SELF_CHECK_FAILED = re.compile(r"^\s*FAIL(ED)?\b", re.M)


def failed_lines(logs_dir):
    """Test name -> first FAILED line of its output, for every log that has one."""
    found = {}
    for log in Path(logs_dir).rglob("*.log"):
        text = log.read_text(errors="replace")
        name = re.search(r"^name: (.*)$", text, re.M)
        hit = SELF_CHECK_FAILED.search(text)
        if name and hit:
            end = text.find("\n", hit.start())
            found[name.group(1)] = text[hit.start() : end if end >= 0 else None].strip()
    return found


def outcome(row, failed):
    if row["Pass"] == "True":
        return "self-check" if row["TestName"] in failed else "pass"
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
    failed = failed_lines(sys.argv[2])
    by_class = {}
    for row in rows:
        by_class.setdefault(outcome(row, failed), []).append(row)
    counts = Counter({k: len(v) for k, v in by_class.items()})

    print(f"## sv-tests: {counts['pass']}/{len(rows)} pass\n")
    print("| outcome | tests |\n|---|---|")
    for name in ("pass", "self-check", "crash", "timeout", "ran", "rejected"):
        print(f"| {name} | {counts[name]} |")
    for name in ("crash", "timeout", "ran", "self-check"):
        listed = sorted(by_class.get(name, []), key=lambda r: r["TestName"])
        if not listed:
            continue
        print(f"\n### {name}\n")
        for row in listed[:LISTED]:
            detail = failed.get(row["TestName"]) if name == "self-check" else f"exit {row['ExitCode']}"
            print(f"- `{row['TestName']}` ({detail})")
        if len(listed) > LISTED:
            print(f"- ... {len(listed) - LISTED} more")
    return 1 if counts["crash"] else 0


if __name__ == "__main__":
    sys.exit(main())
