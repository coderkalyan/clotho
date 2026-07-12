#!/bin/sh
# Fetch pinned cv32e40p RTL + CoreMark sources, stage a flat build in ./src,
# and build the CoreMark firmware (coremark.hex).
#
# The core is the RV32IMC integer configuration (FPU=0, cv32e40p_manifest.flist
# minus the RVFI tracer wrapper). The example testbench modules that Clotho
# cannot lower verbatim (SV classes / SVA / DPI export / string $value$plusargs)
# are patched and vendored under ./tb; the remaining unpatched TB helpers
# (amo_shim, the stall models, perturbation_pkg) are staged from upstream here.
#
# CoreMark is driven bare-metal: the port (fw/port) times with the mcycle CSR
# and prints via CoreMark's own ee_printf routed to the testbench char register
# (0x1000_0000); fw/bsp is a freestanding crt0/linker + libnano (no newlib).
set -eu

CORE_URL=https://github.com/openhwgroup/cv32e40p
CORE_SHA=6033d2b1be3295ec774d17ac4cf226faacfdeb08
CM_URL=https://github.com/eembc/coremark
CM_SHA=1f483d5b8316753a742cbf5590caf5bd0a4e4777

cd "$(dirname "$0")"
rm -rf src _core fw/cm

git init -q _core
git -C _core remote add origin "$CORE_URL"
git -C _core fetch -q --depth 1 origin "$CORE_SHA"
git -C _core checkout -q FETCH_HEAD

mkdir src
# Integer core fileset (manifest, minus the tracer tb_wrapper which pulls file I/O).
for f in $(sed "s#\${DESIGN_RTL_DIR}#_core/rtl#g" _core/cv32e40p_manifest.flist \
             | grep -E '\.sv' | grep -v cv32e40p_tb_wrapper); do
	cp "$f" src/
done
# Unpatched example-testbench helpers (the patched TB modules live in ./tb).
for f in amo_shim riscv_rvalid_stall riscv_gnt_stall; do
	cp "_core/example_tb/core/$f.sv" src/
done
cp _core/example_tb/core/include/perturbation_pkg.sv src/
rm -rf _core

# CoreMark + firmware.
git init -q fw/cm
git -C fw/cm remote add origin "$CM_URL"
git -C fw/cm fetch -q --depth 1 origin "$CM_SHA"
git -C fw/cm checkout -q FETCH_HEAD
if command -v riscv32-unknown-elf-gcc >/dev/null 2>&1; then
	ITER="${ITER:-10}" sh fw/build_fw.sh
	cp fw/coremark.hex coremark.hex
else
	echo "note: riscv32-unknown-elf-gcc not found; using the committed coremark.hex"
fi

echo "staged $(ls src/*.sv | wc -l) core files + $(ls tb/*.sv | wc -l) TB files + coremark.hex"
