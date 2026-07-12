# Freestanding reset + trap stub for the Hazard3 CoreMark firmware.
#
# Hazard3's reset vector is 0x80000040 and mtvec base 0x80000000 (config.vh
# RESET_VECTOR / MTVEC_INIT), so a 16-entry vector table at 0x80000000 places
# _reset exactly at +0x40. crt0 duties beyond the existing hazard3 stub:
# initialize gp (for .sdata/.sbss relaxation) and zero .bss before CoreMark's
# static result structs are used. Traps print '!' and exit nonzero so a core
# bug surfaces as a failed run, not a silent hang.

	.section .vectors, "ax"
	.option norvc
	.p2align 12
	.global _vectors
_vectors:
	# 16 entries, 4 bytes each => _reset at +0x40.
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap
	j _trap

	.global _reset
_reset:
	la   sp, __stack_top
.option push
.option norelax
	la   gp, __global_pointer$
.option pop
	la   t0, _vectors
	csrw mtvec, t0             # direct mode: all traps -> _vectors[0]

	# Zero .bss (CoreMark's static results / MEM_STATIC pool live here).
	la   a0, __bss_start
	la   a1, __bss_end
1:	bgeu a0, a1, 2f
	sw   zero, 0(a0)
	addi a0, a0, 4
	j    1b
2:
	call main
	li   t1, 0xc0000008        # IO_EXIT
	sw   a0, 0(t1)
3:	j 3b

	.option rvc
	.p2align 2
	.global _trap
_trap:
	li   t1, 0xc0000000        # IO_PRINT_CHAR
	li   t2, 0x21              # '!'
	sw   t2, 0(t1)
	li   t1, 0xc0000008        # IO_EXIT
	li   a0, -1
	sw   a0, 0(t1)
4:	j 4b
