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
crashed (docs/VERIFICATION.md: never crash); the other classes only report.

sv-tests-known.txt, next to this script, lists tests whose self-check or ran
outcome is understood and is not a Clotho defect to fix now. A listed test
that still has its listed outcome is reported in its own section and is left
out of the self-check and ran counts, so those counts hold only what has not
been looked at. A listed test whose outcome has changed (it passes, or fails
some other way, or is gone from the report) is reported as an out-of-date
entry, so that the list does not outlive the reason it was written.
"""

import csv
import re
import sys
from collections import Counter
from pathlib import Path

TIMEOUT_EXIT = 71
LISTED = 50
KNOWN_LIST = Path(__file__).with_name("sv-tests-known.txt")
# Known category -> the outcome it explains.
KNOWN_OUTCOME = {"race": "self-check", "oracle": "self-check", "known-ran": "ran"}
KNOWN_TITLE = {
    "race": "race (LRM 4.7: order unspecified; for manual review)",
    "oracle": "oracle (the test's expectation contradicts the LRM)",
    "known-ran": "known-ran (exit 0, test expects failure or its check cannot pass)",
}
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


def load_known(path):
    """Test name -> (category, reason), from lines of `category name reason`."""
    known = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = line.split(None, 2)
        if len(fields) != 3 or fields[0] not in KNOWN_OUTCOME or fields[1] in known:
            sys.exit(f"{path}:{number}: expected `<{'|'.join(KNOWN_OUTCOME)}> <unique test name> <reason>`")
        known[fields[1]] = (fields[0], fields[2])
    return known


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
    known = load_known(KNOWN_LIST)
    by_class = {}
    by_known = {}
    stale = []
    seen = set()
    for row in rows:
        name = row["TestName"]
        seen.add(name)
        result = outcome(row, failed)
        entry = known.get(name)
        if entry and KNOWN_OUTCOME[entry[0]] == result:
            by_known.setdefault(entry[0], []).append(row)
            continue
        by_class.setdefault(result, []).append(row)
        if entry:
            stale.append((name, f"listed {entry[0]} but now {result}"))
    stale += [(name, "listed but not in the report") for name in known if name not in seen]
    counts = Counter({k: len(v) for k, v in by_class.items()})

    print(f"## sv-tests: {counts['pass']}/{len(rows)} pass\n")
    print("| outcome | tests |\n|---|---|")
    for name in ("pass", "self-check", "crash", "timeout", "ran", "rejected"):
        print(f"| {name} | {counts[name]} |")
    for name in KNOWN_OUTCOME:
        print(f"| known {name.removeprefix('known-')} | {len(by_known.get(name, []))} |")
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
    for category in KNOWN_OUTCOME:
        listed = sorted(by_known.get(category, []), key=lambda r: r["TestName"])
        if listed:
            print(f"\n### {KNOWN_TITLE[category]}\n")
            for row in listed:
                print(f"- `{row['TestName']}`: {known[row['TestName']][1]}")
    if stale:
        print("\n### known list out of date\n")
        print(f"Fix or remove these entries in `{KNOWN_LIST.name}`:\n")
        for name, why in sorted(stale):
            print(f"- `{name}`: {why}")
    return 1 if counts["crash"] else 0


if __name__ == "__main__":
    sys.exit(main())
