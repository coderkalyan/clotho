# Minimal freestanding reset + trap stub for the Hazard3 perf testbench.
#
# The core's reset vector is 0x80000040 and mtvec base is 0x80000000 (see
# config.vh RESET_VECTOR / MTVEC_INIT). The vector table below places 16
# word-sized entries at 0x80000000..0x8000003c so that _reset lands exactly at
# 0x80000040. All traps route to _trap, which prints '!' and exits nonzero so a
# core bug surfaces as a failed run rather than a silent hang.

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
	la   t0, _vectors
	csrw mtvec, t0              # direct mode: all traps -> _vectors[0]
	call main
	li   t1, 0xc0000008        # IO_EXIT
	sw   a0, 0(t1)
1:	j 1b

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
2:	j 2b
