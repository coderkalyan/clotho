#!/usr/bin/env python3
"""Permute the declaration order of module-level always blocks in a Verilog file.

Throwaway experiment tooling. Finds `always` at module scope (heuristic: the
keyword starts a line at one-tab indent, matching picorv32 style, so
generate-nested blocks are left alone), extracts each block by token scan
(balanced begin/end and case/endcase outside comments/strings; single-statement
bodies end at the first `;` at depth 0, with else lookahead).

Both slang and iverilog enforce declaration-before-use, so a block may only be
placed after every module-level declaration it references. Permutation runs
constrained random swaps from the identity: swap two blocks iff both still sit
after their latest referenced declaration. Input should be preprocessed
(iverilog -E) so `ifdef`-disabled blocks are gone. Verify semantics by diffing
simulator output.
"""

import random
import re
import sys

TOKEN = re.compile(r"//[^\n]*|/\*.*?\*/|\"(?:[^\"\\]|\\.)*\"|[A-Za-z_$][A-Za-z0-9_$]*|.", re.S)
DECL = re.compile(
    r"^\s*(?:output\s+|input\s+|inout\s+)?"
    r"(?:reg|wire|integer|localparam|parameter|genvar|real|event)\b([^;]*);",
    re.M,
)
FUNC = re.compile(r"^\s*function\b[^;(]*?([A-Za-z_$][A-Za-z0-9_$]*)\s*[(;]", re.M)
TASK = re.compile(r"^\s*task\s+([A-Za-z_$][A-Za-z0-9_$]*)", re.M)
IDENT = re.compile(r"[A-Za-z_$][A-Za-z0-9_$]*")


def block_end(text: str, start: int) -> int:
    """Return index one past the end of the always construct at `start`."""
    toks = [
        (m.group(0), m.end())
        for m in TOKEN.finditer(text, start)
        if not (m.group(0).startswith("//") or m.group(0).startswith("/*") or m.group(0).isspace())
    ]
    depth = 0
    paren = 0
    i = 0
    while i < len(toks):
        t, end = toks[i]
        i += 1
        if t.startswith('"'):
            continue
        if t == "(":
            paren += 1
            continue
        if t == ")":
            paren -= 1
            continue
        if paren > 0:
            continue
        done = False
        if t == "begin" or t in ("case", "casez", "casex"):
            depth += 1
        elif t in ("end", "endcase"):
            depth -= 1
            done = depth == 0
        elif t == ";" and depth == 0:
            done = True
        if done:
            # `if ... end/;` may continue with an else branch.
            if i < len(toks) and toks[i][0] == "else":
                continue
            return end
    raise SystemExit(f"unterminated always block at {start}")


def decl_positions(text: str, ms: int, me: int) -> dict[str, int]:
    """Map each module-level declared name to the end offset of its declaration."""
    pos: dict[str, int] = {}
    for m in DECL.finditer(text, ms, me):
        names = m.group(1)
        # Strip ranges/dims and initializers, keep declared identifiers.
        names = re.sub(r"\[[^]]*\]", " ", names)
        for part in names.split(","):
            part = part.split("=")[0]
            ids = IDENT.findall(part)
            if ids:
                pos[ids[-1]] = max(pos.get(ids[-1], 0), m.end())
    for rx in (FUNC, TASK):
        for m in rx.finditer(text, ms, me):
            pos[m.group(1)] = max(pos.get(m.group(1), 0), block_like_end(text, m.start()))
    return pos


def block_like_end(text: str, start: int) -> int:
    """End of a function/task: the matching endfunction/endtask."""
    m = re.compile(r"\bend(?:function|task)\b").search(text, start)
    return m.end() if m else start


def used_idents(text: str, s: int, e: int) -> set[str]:
    ids = set()
    for m in TOKEN.finditer(text, s):
        if m.end() > e:
            break
        t = m.group(0)
        if t[0].isalpha() or t[0] in "_$":
            ids.add(t)
    return ids


def permute(text: str, seed: int) -> str:
    modules = []
    for m in re.finditer(r"^module\b", text, re.M):
        e = text.find("\nendmodule", m.start())
        modules.append((m.start(), e))

    rng = random.Random(seed)
    out = []
    pos = 0
    total_moved = 0
    for ms, me in modules:
        spans = [
            (m.start(), block_end(text, m.start()))
            for m in re.finditer(r"^\talways\b", text, re.M)
            if ms <= m.start() < me
        ]
        if len(spans) < 2:
            continue
        decls = decl_positions(text, ms, me)
        # Earliest offset each block may be placed at: end of the latest
        # module-level declaration it references (0 if only header names).
        earliest = [
            max((decls.get(i, 0) for i in used_idents(text, s, e)), default=0)
            for s, e in spans
        ]
        # Constrained random swaps from identity: perm[k] = block occupying slot k.
        perm = list(range(len(spans)))
        for _ in range(len(spans) * 30):
            a, b = rng.randrange(len(spans)), rng.randrange(len(spans))
            if a == b:
                continue
            if earliest[perm[a]] <= spans[b][0] and earliest[perm[b]] <= spans[a][0]:
                perm[a], perm[b] = perm[b], perm[a]
        total_moved += sum(1 for k, p in enumerate(perm) if k != p)
        for k, (s, e) in enumerate(spans):
            out.append(text[pos:s])
            ps, pe = spans[perm[k]]
            out.append(text[ps:pe])
            pos = e
    out.append(text[pos:])
    print(f"moved {total_moved} always blocks", file=sys.stderr)
    return "".join(out)


if __name__ == "__main__":
    path, seed = sys.argv[1], int(sys.argv[2])
    with open(path) as f:
        text = f.read()
    sys.stdout.write(permute(text, seed))
