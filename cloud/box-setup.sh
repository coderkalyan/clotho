#!/usr/bin/env bash
# Differential-benchmark box setup: install measurement deps and quiet the
# hardware so run-to-run timing is reproducible. Idempotent. Run as root
# (cloud-init user-data, or by hand). clotho itself ships prebuilt+static, so
# there is deliberately NO LLVM / cmake / build toolchain here.
set -euo pipefail

# --- deps: measurement harness + iverilog and verilator only ---
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y --no-install-recommends \
  python3 hyperfine iverilog verilator ca-certificates rsync
# NOTE: iverilog/verilator are apt's versions; perf.py records the exact version
# per run. They are non-gating reference context — the gated metric is
# clotho(base) vs clotho(head), which is toolchain-independent here.

# --- quiet hardware: kill the sources of one-sided timing noise ---
# performance governor: hold max frequency, no DVFS ramp between samples
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  [ -w "$g" ] && echo performance > "$g" || true
done
# turbo/boost off: opportunistic boost is the biggest run-to-run variance source
if [ -w /sys/devices/system/cpu/intel_pstate/no_turbo ]; then
  echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo
elif [ -w /sys/devices/system/cpu/cpufreq/boost ]; then
  echo 0 > /sys/devices/system/cpu/cpufreq/boost      # AMD path
fi
# SMT off: a sibling thread sharing the core perturbs a pinned single-thread bench
[ -w /sys/devices/system/cpu/smt/control ] && { echo off > /sys/devices/system/cpu/smt/control || true; }
# stable code layout across runs
[ -w /proc/sys/kernel/randomize_va_space ] && echo 0 > /proc/sys/kernel/randomize_va_space || true
# no THP defrag stalls mid-measurement
[ -w /sys/kernel/mm/transparent_hugepage/defrag ] && echo never > /sys/kernel/mm/transparent_hugepage/defrag || true

# --- report the environment so results are interpretable ---
echo "== box-setup done =="
echo "cpu:      $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo n/a)"
echo "turbo:    $(cat /sys/devices/system/cpu/intel_pstate/no_turbo 2>/dev/null && echo '(1=off)' || echo n/a)"
echo "smt:      $(cat /sys/devices/system/cpu/smt/control 2>/dev/null || echo n/a)"
echo "iverilog: $(iverilog -V 2>&1 | head -1)"
echo "verilator:$(verilator --version 2>&1 | head -1)"
# NOTE: isolcpus/nohz_full need a kernel cmdline (set via the image/boot config,
# not at runtime) — the runner pins the bench to a fixed core with taskset instead.
