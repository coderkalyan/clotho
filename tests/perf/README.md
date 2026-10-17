# Performance benchmarks

Tracks clotho compile time and simulation time per benchmark case, gates
regressions against a baseline measured on the same machine, and records
iverilog and Verilator numbers on the same cases for context. Requires
`hyperfine` and Python 3.11+.

Timings are only comparable on one machine, so `baseline.json` and
`oracles.json` are written locally by the commands below and are not part of
the repository.

## Usage

```sh
tests/perf/perf.py run                # benchmark -> results.json (gitignored)
tests/perf/perf.py compare            # gate results.json vs baseline.json
tests/perf/perf.py baseline           # re-measure and rewrite baseline.json (sign-off)
tests/perf/perf.py oracles            # measure iverilog/verilator -> oracles.json
```

All subcommands take `--runs N` (default 5) and `--cases NAME...`; `run` and
`baseline` take `--no-build`; `oracles` takes `--tools TOOL...`.

## Methodology

- Clotho is built `-DCMAKE_BUILD_TYPE=Release` into `build-perf/` (the harness
  configures/builds it automatically; the runtime is always optimized anyway).
- Every timing is hyperfine with 1 warmup + N runs (default 5); the reported
  number is the **minimum** — run-time noise is one-sided (log-normal-ish), so
  min is the best estimator of the true cost.
- One hyperfine invocation per case measures the warm-cache wall clock; the
  command appends `--time` stderr to `splits.txt`, giving the per-run
  read-source / analyze / compile / simulate splits from the same runs.
  `compile_ms` = read source + analyze + compile (everything before simulation).
- Each case is sanity-run once before timing (exit 0 + `check.stdout_regex`
  match); a broken sim is never benchmarked.

## Regression policy

`compare` flags metric `M` as regressed when
`new_min > base_min * (1 + margin)` with
`margin = max(2%, 2 * max(spread_base, spread_new))` where
`spread = median/min - 1` of the run samples — a robust one-sided noise
measure (a single slow outlier run widens stddev by an order of magnitude but
leaves `median - min` untouched), so noisy measurements widen the gate
instead of false-failing.

- **`sim_ms` regression on any case is a bug.** Fix before merging.
- **`compile_ms` / `wall_ms` regressions need manual sign-off.**

Sign-off = run `perf.py baseline` after reviewing the regression, so the new
numbers become the reference for later runs on that machine. `compare` exits 1
on any regression, so a future CI job is `perf.py run && perf.py compare` on a
pinned runner that keeps its own baseline.

## Oracles

`perf.py oracles` measures iverilog and Verilator compile and sim time per
case, writing `oracles.json` (with tool versions, host and date). It is
run manually when convenient — never in CI — and `compare` only prints the
numbers as context, never gates on them. Tools missing from PATH are skipped,
keeping existing numbers. Per-case, per-tool applicability lives in the case's
`case.toml` (`[oracles.<tool>] enabled = false` + `note`), e.g. a feature
iverilog does not support, or a harness that is not workload-comparable.

A tool installed outside the system PATH is located via `tools.local.json`
(gitignored; copy `tools.local.json.example`), a `{tool: bindir}` map prepended
to PATH for that tool's commands. A tool the shell already resolves needs no
entry.

## Cases

A case is a directory under `cases/` with a `case.toml`:

```toml
name = "picorv32"
top = "testbench"                     # top module, used by oracle recipes
[fetch]                               # optional: fetch sources on demand
script = "fetch.sh"                   # pinned shallow clone into src/ (gitignored)
probe = "src/picorv32.v"              # fetch runs when this file is missing
[clotho]
files = ["src/picorv32.v", "testbench.v"]
args = []                             # extra clotho flags (e.g. "-O")
[check]
stdout_regex = "TIMEOUT"              # sanity gate before any timing
[oracles.verilator]
enabled = false                       # per-case tool disable
note = "why"
files = ["..."]                       # optional per-tool file list override
```

Commands run with the case directory as cwd (relative paths like
`firmware/firmware.hex` resolve there).

### picorv32

Stock `picorv32.v` (pinned YosysHQ/picorv32 @ 87c89ac via `fetch.sh`) plus a
vendored `testbench.v` from the same SHA with the `$test$plusargs` /
`$value$plusargs` sites disabled (clotho TODO); runs to the testbench's
1M-cycle watchdog `TIMEOUT`, a fixed deterministic cycle count.
`firmware/firmware.hex` is a 16-instruction program written for Clotho (a delay
loop that prints `OK` and writes the testbench's pass flag; source in `firmware.S`), checked in so the
case needs no toolchain. Verilator is disabled as an oracle: its harness drives the clock
from C++ without AXI delay randomization and ends at firmware completion —
a different workload.
