#!/bin/sh
# Regenerate the design from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py combchain --n 512 --cycles 20000 > design.sv
