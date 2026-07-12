#!/bin/sh
# Build CoreMark for the bare-metal Hazard3 core (RV32IMC subset of IMACB; no
# newlib/libgcc, self-contained ee_printf + libnano). Expects CoreMark sources
# in ./cm (staged by ../fetch.sh). ITER overridable (default 60 ~= 5M cycles).
set -eu
cd "$(dirname "$0")"
ITER="${ITER:-60}"
PFX=riscv32-unknown-elf-
# rv32imc is a safe firmware subset of Hazard3's RV32IMACB; _zicsr for csrr.
COMMON="-march=rv32imc_zicsr -mabi=ilp32 -Os -g -nostdlib -ffreestanding -fno-builtin"

# Route CoreMark's ee_printf to the tb_hazard3 char register (idempotent).
if grep -q '#error "You must implement the method uart_send_char' cm/barebones/ee_printf.c 2>/dev/null; then
  sed -i 's|#error "You must implement the method uart_send_char to use this file!\\n";|*(volatile int *)0xc0000000 = (int)(unsigned char)c;|' cm/barebones/ee_printf.c
fi

${PFX}gcc $COMMON \
  -DITERATIONS=$ITER -DPERFORMANCE_RUN=1 '-DFLAGS_STR="-Os rv32imc"' \
  -I port -I cm -I cm/barebones \
  -T bsp/link.ld \
  bsp/crt0.S bsp/libnano.c \
  cm/core_list_join.c cm/core_main.c cm/core_matrix.c cm/core_state.c cm/core_util.c \
  cm/barebones/ee_printf.c \
  port/core_portme.c \
  -o coremark.elf

# Little-endian 32-bit words, one hex word per line, for $readmemh (loaded at
# MEM_BASE=0x80000000). objcopy -O verilog emits byte records addressed from 0,
# but the firmware links at 0x80000000; a flat binary + word repack keeps the
# hex indexed from mem[0] == MEM_BASE.
${PFX}objcopy -O binary coremark.elf coremark.bin
python3 - coremark.bin coremark.hex <<'PY'
import sys, struct
data = open(sys.argv[1], "rb").read()
data += b"\x00" * ((-len(data)) % 4)
with open(sys.argv[2], "w") as f:
    for i in range(0, len(data), 4):
        (w,) = struct.unpack_from("<I", data, i)
        f.write("%08x\n" % w)
PY
echo "wrote coremark.hex ($(wc -l < coremark.hex) words), ITER=$ITER"
