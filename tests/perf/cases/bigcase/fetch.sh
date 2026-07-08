#!/bin/sh
# Regenerate the design from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py bigcase --n 2048 --cycles 200000 > design.sv
