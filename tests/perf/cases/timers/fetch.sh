#!/bin/sh
# Regenerate the design from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py timers --n 512 --cycles 2000000 > design.sv
