#!/bin/sh
# Build CoreMark for the stock picorv32 core (RV32IM, NO compressed ISA:
# COMPRESSED_ISA=0 in the testbench) as a bare-metal image. No newlib/libgcc:
# hardware mul/div (rv32im) + self-contained ee_printf + libnano. ITER default
# 10 (~5M cycles). Emits a 32-bit-word hex (picorv32 $readmemh format), not the
# byte-oriented "objcopy -O verilog" format the word-addressed RAM cannot load.
set -eu
cd "$(dirname "$0")"
ITER="${ITER:-10}"
PFX=riscv32-unknown-elf-
# rv32im (mul/div in hw) + zicsr (rdcycle); NO 'c' — picorv32 has no compressed ISA.
COMMON="-march=rv32im_zicsr -mabi=ilp32 -Os -g -nostdlib -ffreestanding -fno-builtin -fno-common"

# Route CoreMark's ee_printf to the testbench char register (idempotent).
if grep -q '#error "You must implement the method uart_send_char' cm/barebones/ee_printf.c 2>/dev/null; then
  sed -i 's|#error "You must implement the method uart_send_char to use this file!\\n";|*(volatile int *)0x10000000 = (int)(unsigned char)c;|' cm/barebones/ee_printf.c
fi

${PFX}gcc $COMMON \
  -DITERATIONS=$ITER -DPERFORMANCE_RUN=1 '-DFLAGS_STR="-Os rv32im"' \
  -I port -I cm -I cm/barebones \
  -T bsp/link.ld \
  bsp/crt0.S bsp/libnano.c \
  cm/core_list_join.c cm/core_main.c cm/core_matrix.c cm/core_state.c cm/core_util.c \
  cm/barebones/ee_printf.c \
  port/core_portme.c \
  -o coremark.elf

${PFX}objcopy -O binary coremark.elf coremark.bin
# 128 KiB testbench RAM => 32768 words. Pad to word count, big-endian print of
# each little-endian word (matches picorv32/firmware/makehex.py).
python3 - coremark.bin 32768 > coremark.hex <<'PY'
import sys
data = open(sys.argv[1], "rb").read()
nwords = int(sys.argv[2])
assert len(data) % 4 == 0, len(data)
assert len(data) < 4 * nwords, (len(data), nwords)
for i in range(len(data) // 4):
    w = data[4*i:4*i+4]
    print("%02x%02x%02x%02x" % (w[3], w[2], w[1], w[0]))
PY
echo "wrote coremark.hex ($(wc -l < coremark.hex) words, .bin $(wc -c < coremark.bin) bytes), ITER=$ITER"
