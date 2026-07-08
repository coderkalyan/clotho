# Source of firmware.hex: the program the picorv32 case runs. It spins for a
# fixed number of cycles, prints "OK\n" through the testbench's character
# register, and signals completion through its pass register.
#
# Build: assemble for RV32I, take the .text bytes, and write one little-endian
# 32-bit word per line in hex, padded with zero words to the testbench's 32768-word
# memory:
#
#   llvm-mc --triple=riscv32 -filetype=obj firmware.S -o firmware.o
#   llvm-objcopy -O binary --only-section=.text firmware.o firmware.bin
#   python3 -c 'import sys,struct; b=open("firmware.bin","rb").read(); \
#     w=[struct.unpack_from("<I",b,i)[0] for i in range(0,len(b),4)]; \
#     print("\n".join(["%08x"%x for x in w]+["0"]*(32768-len(w))))' > firmware.hex

    .option norvc
    .text
    .globl _start
_start:
    lui     a0, 0x10000          # a0 = 0x1000_0000, the character register
    lui     t0, 0x31
    addi    t0, t0, -704         # t0 = 200000 iterations
delay:
    addi    t0, t0, -1
    bnez    t0, delay
    addi    a1, zero, 'O'
    sw      a1, 0(a0)
    addi    a1, zero, 'K'
    sw      a1, 0(a0)
    addi    a1, zero, '\n'
    sw      a1, 0(a0)
    lui     a0, 0x20000          # a0 = 0x2000_0000, the pass register
    lui     a1, 0x75bd
    addi    a1, a1, -747         # a1 = 123456789, the pass value
    sw      a1, 0(a0)
    ebreak
