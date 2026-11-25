#!/usr/bin/env python3
"""Classify an sv-tests report for clotho and fail on anything unexplained.

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
  trace       the run broke a rule of the scheduler: the trace check of a build with
              CLOTHO_EXTRA_ASSERTS (the linux-x64 binary has it) printed
              `clotho: scheduler trace violation:` in the log. Read before pass and
              rejected, since the run ended early with an exit status of its own
              (docs/design/scheduler-trace.md)

self-check and ran are where silent miscompiles show up. Exits 1 on any crash or
timeout, on any self-check, ran or trace outcome not explained by the known list,
and on an out-of-date known entry, so a nightly that exits 0 has no unexplained
failure (docs/VERIFICATION.md).

sv-tests-known.txt, next to this script, lists tests whose self-check, ran or
trace outcome is understood and is not a Clotho defect to fix now. A listed test
that still has its listed outcome is reported in its own section and is left
out of the self-check, ran and trace counts, so those counts hold only what has
not been looked at. A listed test whose outcome has changed (it passes, or fails
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
KNOWN_OUTCOME = {"race": "self-check", "oracle": "self-check", "known-ran": "ran", "trace": "trace"}
KNOWN_TITLE = {
    "race": "race (LRM 4.7: order unspecified; reviewed, no design may rely on it)",
    "oracle": "oracle (the test's expectation contradicts the LRM)",
    "known-ran": "known-ran (exit 0, test expects failure or its check cannot pass)",
    "trace": "trace (a rule of the scheduler is broken: a Clotho bug or a rule to reword)",
}
SELF_CHECK_FAILED = re.compile(r"^\s*FAIL(ED)?\b", re.M)
# What `clotho run` prints, in a build with the scheduler trace check, when a run breaks a rule
# (docs/design/scheduler-trace.md); the next line names the rule and what broke it.
TRACE_VIOLATION = re.compile(r"^clotho: scheduler trace violation:\n(.*)$", re.M)


def log_hits(logs_dir, pattern, group=0):
    """Test name -> the first match of `pattern` (its `group`) in its output, for every log that has one."""
    found = {}
    for log in Path(logs_dir).rglob("*.log"):
        text = log.read_text(errors="replace")
        name = re.search(r"^name: (.*)$", text, re.M)
        hit = pattern.search(text)
        if name and hit:
            end = text.find("\n", hit.start(group))
            found[name.group(1)] = text[hit.start(group) : end if end >= 0 else None].strip()
    return found


def failed_lines(logs_dir):
    """Test name -> first FAILED line of its output, for every log that has one."""
    return log_hits(logs_dir, SELF_CHECK_FAILED)


def trace_lines(logs_dir):
    """Test name -> the rule line of its trace violation, for every log that has one."""
    return log_hits(logs_dir, TRACE_VIOLATION, 1)


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


def outcome(row, failed, traced):
    # A run the trace check ended is a violation whatever the harness made of its exit status.
    if row["TestName"] in traced:
        return "trace"
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
    traced = trace_lines(sys.argv[2])
    known = load_known(KNOWN_LIST)
    by_class = {}
    by_known = {}
    stale = []
    seen = set()
    for row in rows:
        name = row["TestName"]
        seen.add(name)
        result = outcome(row, failed, traced)
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
    for name in ("pass", "self-check", "crash", "timeout", "ran", "rejected", "trace"):
        print(f"| {name} | {counts[name]} |")
    for name in KNOWN_OUTCOME:
        print(f"| known {name.removeprefix('known-')} | {len(by_known.get(name, []))} |")
    for name in ("crash", "timeout", "ran", "self-check", "trace"):
        listed = sorted(by_class.get(name, []), key=lambda r: r["TestName"])
        if not listed:
            continue
        print(f"\n### {name}\n")
        for row in listed[:LISTED]:
            detail = {"self-check": failed, "trace": traced}.get(name, {}).get(row["TestName"], f"exit {row['ExitCode']}")
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
    unexplained = counts["crash"] + counts["timeout"] + counts["self-check"] + counts["ran"] + counts["trace"]
    return 1 if unexplained or stale else 0


if __name__ == "__main__":
    sys.exit(main())
