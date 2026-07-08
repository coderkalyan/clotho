#!/bin/sh
# Regenerate the design from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py thunk --n 256 --cycles 100000 --shape comb > design.sv
