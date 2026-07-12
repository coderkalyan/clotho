#!/bin/sh
# Build CoreMark for a bare-metal RV32IMC core (no newlib/libgcc: hardware
# mul/div, self-contained ee_printf + libnano). Expects CoreMark sources in
# ./cm (staged by ../fetch.sh). ITER overridable (default 10 ~= 5M cycles).
set -eu
cd "$(dirname "$0")"
ITER="${ITER:-10}"
PFX=riscv32-unknown-elf-
COMMON="-march=rv32imc_zicsr -mabi=ilp32 -Os -g -nostdlib -ffreestanding -fno-builtin"

# Route CoreMark's ee_printf to the testbench char register (idempotent).
if grep -q '#error "You must implement the method uart_send_char' cm/barebones/ee_printf.c 2>/dev/null; then
  sed -i 's|#error "You must implement the method uart_send_char to use this file!\\n";|*(volatile int *)0x10000000 = (int)(unsigned char)c;|' cm/barebones/ee_printf.c
fi

${PFX}gcc $COMMON \
  -DITERATIONS=$ITER -DPERFORMANCE_RUN=1 '-DFLAGS_STR="-Os rv32imc"' \
  -I port -I cm -I cm/barebones \
  -T bsp/link.ld \
  bsp/crt0.S bsp/vectors.S bsp/libnano.c \
  cm/core_list_join.c cm/core_main.c cm/core_matrix.c cm/core_state.c cm/core_util.c \
  cm/barebones/ee_printf.c \
  port/core_portme.c \
  -o coremark.elf
${PFX}objcopy -O verilog coremark.elf coremark.hex
echo "wrote coremark.hex ($(wc -l < coremark.hex) lines), ITER=$ITER"
