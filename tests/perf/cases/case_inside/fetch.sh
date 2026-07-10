#!/bin/sh
# Regenerate the design from the seeded generator (authoritative source).
set -e
python3 ../../../../bench-sched/gen.py case_inside --n 2048 --cycles 200000 --gap 8 > design.sv
