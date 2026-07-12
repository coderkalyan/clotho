#!/bin/sh
# Fetch pinned Hazard3 RTL + CoreMark, stage a flat core build in ./src, and
# build the CoreMark firmware (coremark.hex).
#
# Core staging mirrors the hazard3 perf case: Hazard3's RTL lives in hdl/ with
# headers included across subdirectories, so the CPU file set (hazard3.f) plus
# every .vh header is flattened into src/ where `include "x.vh"` resolves
# same-dir (clotho's slang frontend searches the includer's own directory;
# oracle front ends get -I src via case.toml incdirs). The case-dir testbench
# (tb_hazard3.sv) is compiled directly, so the two headers it includes --
# config_default.vh (copied as config.vh) and hazard3_config_inst.vh -- are
# copied next to it.
#
# CoreMark is driven bare-metal: the port (fw/port) times with the mcycle CSR
# and prints via CoreMark's own ee_printf routed to the tb_hazard3 char register
# (0xc000_0000); fw/bsp is a freestanding crt0/linker + libnano (no newlib).
set -eu

CORE_URL=https://github.com/Wren6991/Hazard3
CORE_SHA=8af992930f71a69b0e06c38734c1094f41a05ca0
CM_URL=https://github.com/eembc/coremark
CM_SHA=1f483d5b8316753a742cbf5590caf5bd0a4e4777

cd "$(dirname "$0")"
rm -rf src _clone fw/cm

git init -q _clone
git -C _clone remote add origin "$CORE_URL"
git -C _clone fetch -q --depth 1 origin "$CORE_SHA"
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

# CoreMark + firmware.
git init -q fw/cm
git -C fw/cm remote add origin "$CM_URL"
git -C fw/cm fetch -q --depth 1 origin "$CM_SHA"
git -C fw/cm checkout -q FETCH_HEAD
if command -v riscv32-unknown-elf-gcc >/dev/null 2>&1; then
	ITER="${ITER:-11}" sh fw/build_fw.sh
	cp fw/coremark.hex coremark.hex
else
	echo "note: riscv32-unknown-elf-gcc not found; using the committed coremark.hex"
fi

echo "staged $(ls src/*.v | wc -l) RTL files + coremark.hex"
