#!/usr/bin/env bash
# Differential benchmark runner — executes ON the quiet box. Consumes two
# prebuilt static clotho binaries (from build-refs.sh) and the shipped
# tests/perf tree; does no building. Pins everything to one isolated core,
# runs each binary over the identical workload, and gates head vs base with
# perf.py's significance policy. Exit code is the gate's (0 clean, 1 regression).
#
#   cloud/bench-remote.sh --base-bin clotho-base --head-bin clotho-head \
#       [--cases NAME...] [--runs 5] [--core 2] [--oracles] [--out bench-report]
set -euo pipefail

SELF=$(cd "$(dirname "$0")" && pwd)
PERF_DIR=${PERF_DIR:-$SELF/../tests/perf}
base_bin= head_bin= runs=5 core=2 oracles=0 out=bench-report
cases=()
while [ $# -gt 0 ]; do
  case "$1" in
    --base-bin) base_bin=$2; shift 2;;
    --head-bin) head_bin=$2; shift 2;;
    --runs)     runs=$2; shift 2;;
    --core)     core=$2; shift 2;;        # -1 disables the pin
    --oracles)  oracles=1; shift;;
    --out)      out=$2; shift 2;;
    --perf-dir) PERF_DIR=$2; shift 2;;
    --cases)    shift; while [ $# -gt 0 ] && [ "${1#--}" = "$1" ]; do cases+=("$1"); shift; done;;
    *) echo "bench-remote: unknown arg '$1'" >&2; exit 2;;
  esac
done

PERF="$PERF_DIR/perf.py"
[ -f "$PERF" ] || { echo "bench-remote: perf.py not found at $PERF (set --perf-dir)" >&2; exit 2; }
for b in "$base_bin" "$head_bin"; do
  [ -x "$b" ] || { echo "bench-remote: binary not executable: $b" >&2; exit 2; }
done
base_bin=$(readlink -f "$base_bin"); head_bin=$(readlink -f "$head_bin")

# taskset pin: with SMT off the chosen core is a whole physical core to itself.
PIN=(); if [ "$core" != "-1" ] && command -v taskset >/dev/null; then PIN=(taskset -c "$core"); fi

mkdir -p "$out"; out=$(readlink -f "$out")
base_json="$out/base.json"; head_json="$out/head.json"
rm -f "$base_json" "$head_json"

sha() { [ -f "$1.sha" ] && cut -c1-12 "$1.sha" || echo "unknown"; }

# --- environment snapshot: results are only interpretable with it ---
{
  echo "== bench-remote environment =="
  echo "date:     $(date -u +%FT%TZ)"
  echo "host:     $(uname -n)  kernel $(uname -r)"
  echo "cpu:      $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
  echo "governor: $(cat /sys/devices/system/cpu/cpu${core}/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
  echo "turbo:    $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null || echo n/a) (1=off)"
  echo "smt:      $(cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo n/a)"
  echo "pin core: ${core}   runs: ${runs}"
  echo "base bin: $base_bin  (sha $(sha "$base_bin"))"
  echo "head bin: $head_bin  (sha $(sha "$head_bin"))"
} | tee "$out/env.txt"

run_one() {
  local label=$1 bin=$2 json=$3
  echo "[bench-remote] $label: $bin"
  "${PIN[@]}" python3 "$PERF" run --clotho "$bin" --out "$json" --runs "$runs" \
    ${cases[@]+--cases "${cases[@]}"}
}

run_one base "$base_bin" "$base_json"
run_one head "$head_bin" "$head_json"

if [ "$oracles" = 1 ]; then
  echo "[bench-remote] oracles (iverilog, verilator)"
  # Oracles benchmark the workload once (binary-independent); written into the
  # perf tree's oracles.json, which compare reads as non-gating context.
  "${PIN[@]}" python3 "$PERF" oracles --tools iverilog verilator --runs "$runs" \
    ${cases[@]+--cases "${cases[@]}"} || echo "[bench-remote] oracles failed (non-fatal)"
fi

echo "[bench-remote] compare (gate)"
set +e
"${PIN[@]}" python3 "$PERF" compare --baseline "$base_json" --results "$head_json" \
  | tee "$out/report.txt"
rc=${PIPESTATUS[0]}
set -e

cp "$PERF_DIR/oracles.json" "$out/" 2>/dev/null || true
echo "[bench-remote] report bundle -> $out (gate rc=$rc)"
exit "$rc"
