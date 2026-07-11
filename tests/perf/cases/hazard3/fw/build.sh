#!/bin/sh
# Build firmware.hex (checked into the case) from start.S + main.c. Requires a
# rv32 GCC toolchain. Emits a $readmemh-format word list loaded at MEM_BASE.
set -eu

cd "$(dirname "$0")"
CROSS=${CROSS_PREFIX:-riscv32-unknown-elf-}
ARCH=rv32imc_zicsr
ABI=ilp32
OUT=../firmware.hex

${CROSS}gcc -march=$ARCH -mabi=$ABI -nostdlib -ffreestanding -Os \
	-Wl,--no-warn-rwx-segments -T link.ld start.S main.c -o fw.elf
${CROSS}objcopy -O binary fw.elf fw.bin
${CROSS}objdump -d fw.elf > fw.dis

# Little-endian 32-bit words, one hex word per line, for $readmemh.
python3 - "$OUT" <<'PY'
import sys, struct
data = open("fw.bin", "rb").read()
data += b"\x00" * ((-len(data)) % 4)
with open(sys.argv[1], "w") as f:
    for i in range(0, len(data), 4):
        (w,) = struct.unpack_from("<I", data, i)
        f.write("%08x\n" % w)
PY
echo "wrote $OUT ($(wc -l < $OUT) words)"
