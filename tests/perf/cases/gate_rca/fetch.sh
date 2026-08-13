#!/bin/sh
# Regenerate from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py gate_rca --n 64 --cycles 100000 > design.sv
