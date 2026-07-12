/* Copyright (c) 2017  SiFive Inc. All rights reserved.
 * Copyright (c) 2019  ETH Zürich and University of Bologna
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the FreeBSD License.   This program is distributed in the hope that
 * it will be useful, but WITHOUT ANY WARRANTY expressed or implied,
 * including the implied warranties of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  A copy of this license is available at
 * http://www.opensource.org/licenses.
 *
 * Modified by Kalyan Sriram for Clotho, 2026: adapted from
 * openhwgroup/cv32e40p@6033d2b1be32 example_tb/core/custom/crt0.S for picorv32
 * (no vector table; exits through the testbench's pass register).
 */

/* Bare-metal crt0 for picorv32 running CoreMark.
   picorv32 resets PC to PROGADDR_RESET (0) and masks all IRQs (irq_mask = ~0),
   so no vector table / IRQ handler is needed. This code must be the first thing
   in .text (the linker places start*(.text) first at address 0).
   Exit: CoreMark's main() returns here; we mark the testbench "tests passed"
   register (write 123456789 to 0x2000_0000) and ebreak, which — with the ebreak
   IRQ masked — traps and lets the testbench finish with ALL TESTS PASSED. */

.section .text.start
.global _start
.type _start, @function
_start:
	/* set stack pointer to top of the 128 KiB testbench RAM */
	li sp, 0x00020000

	/* initialize global pointer (for gp-relative .sdata/.sbss access) */
.option push
.option norelax
	la gp, __global_pointer$
.option pop

	/* clear the .bss segment */
	la a0, __bss_start
	la a2, __bss_end
	sub a2, a2, a0
	li a1, 0
	call memset

	/* call main(argc=0, argv=NULL, envp=NULL) */
	li a0, 0
	li a1, 0
	li a2, 0
	call main

	/* mark the testbench tests-passed register, then trap to finish */
	li a0, 0x20000000
	li a1, 123456789
	sw a1, 0(a0)
1:
	ebreak
	j 1b
.size _start, .-_start
