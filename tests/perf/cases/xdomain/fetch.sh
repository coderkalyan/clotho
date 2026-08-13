#!/bin/sh
# Regenerate the design from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py xdomain --n 128 --cycles 100000 --rounds 4 > design.sv
