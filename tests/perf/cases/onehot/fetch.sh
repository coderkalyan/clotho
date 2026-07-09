#!/bin/sh
# Regenerate the design from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py onehot --n 512 --cycles 200000 --order lsb > design.sv
