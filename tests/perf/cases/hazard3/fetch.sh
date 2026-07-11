#!/bin/sh
# Fetch pinned Hazard3 sources and stage a flat build dir in ./src.
#
# Hazard3's RTL lives in hdl/ with headers included across subdirectories
# (arith/*.v pulls hdl/*.vh), which needs an include search path. We flatten
# the CPU file set (hazard3.f) plus every .vh header into src/ where
# `include "x.vh"` resolves same-dir (clotho's slang frontend searches the
# includer's own directory; the oracle front ends get -I src via case.toml's
# incdirs). The vendored testbench (tb_hazard3.sv) stays in the case dir and is
# compiled directly, so the two headers it includes -- the all-extensions
# config (config_default.vh, copied in as config.vh) and the parameter
# passthrough (hazard3_config_inst.vh) -- are copied next to it. firmware.hex
# is loaded by the testbench from the case dir at run time, so it stays put.
set -eu

URL=https://github.com/Wren6991/Hazard3
SHA=8af992930f71a69b0e06c38734c1094f41a05ca0

cd "$(dirname "$0")"
rm -rf src _clone
git init -q _clone
git -C _clone remote add origin "$URL"
git -C _clone fetch -q --depth 1 origin "$SHA"
git -C _clone checkout -q FETCH_HEAD

mkdir src
# CPU file set from hazard3.f (paths relative to hdl/).
for f in $(grep '^file ' _clone/hdl/hazard3.f | awk '{print $2}'); do
	cp "_clone/hdl/$f" src/
done
# All headers (config, config_inst, width_const, ops, csr_addr, rv_opcodes...).
cp _clone/hdl/*.vh src/
# Headers the case-dir testbench includes directly.
cp _clone/test/sim/tb_common/hdl/config_default.vh config.vh
cp _clone/hdl/hazard3_config_inst.vh hazard3_config_inst.vh

rm -rf _clone
echo "staged $(ls src/*.v | wc -l) RTL files in src/"
