#!/bin/sh
# Fetch the pinned picorv32 core + CoreMark sources and build the CoreMark
# firmware (coremark.hex). The core is the stock picorv32.v (RV32IM, no
# compressed ISA) driven by the vendored AXI testbench (./testbench.v). CoreMark
# is bare-metal: fw/port times with rdcycle (picorv32 has no mcycle/mcountinhibit
# CSR) and prints via CoreMark's own ee_printf routed to the testbench char
# register (0x1000_0000); fw/bsp is a freestanding crt0/linker + libnano (no
# newlib), with the exit path trapping via ebreak into the testbench.
set -eu

CORE_URL=https://github.com/YosysHQ/picorv32
CORE_SHA=87c89acc18994c8cf9a2311e871818e87d304568
CM_URL=https://github.com/eembc/coremark
CM_SHA=1f483d5b8316753a742cbf5590caf5bd0a4e4777

cd "$(dirname "$0")"
rm -rf src _core fw/cm

# picorv32 core (only picorv32.v is needed; testbench.v is vendored/patched).
git init -q _core
git -C _core remote add origin "$CORE_URL"
git -C _core fetch -q --depth 1 origin "$CORE_SHA"
git -C _core checkout -q FETCH_HEAD
mkdir src
cp _core/picorv32.v src/
rm -rf _core

# CoreMark + firmware.
git init -q fw/cm
git -C fw/cm remote add origin "$CM_URL"
git -C fw/cm fetch -q --depth 1 origin "$CM_SHA"
git -C fw/cm checkout -q FETCH_HEAD
if command -v riscv32-unknown-elf-gcc >/dev/null 2>&1; then
	ITER="${ITER:-3}" sh fw/build_fw.sh
	cp fw/coremark.hex coremark.hex
else
	echo "note: riscv32-unknown-elf-gcc not found; using the committed coremark.hex"
fi

echo "staged src/picorv32.v + coremark.hex"
