/* Copyright (C) 2014-2018 Free Software Foundation, Inc.
   Copyright (C) 2019 ETH Zürich and University of Bologna
   Copying and distribution of this script, with or without modification,
   are permitted in any medium without royalty provided the copyright
   notice and this notice are preserved.

   Modified by Kalyan Sriram for Clotho, 2026: adapted from
   openhwgroup/cv32e40p@6033d2b1be32 example_tb/core/custom/link.ld (itself
   derived from the RISC-V GNU ld default script) for the Hazard3 memory map. */

/* Linker script for the Hazard3 CoreMark firmware.
 * Places the reset vector table at MEM_BASE (0x80000000) so _reset lands at
 * the core's 0x80000040 reset vector; everything fits in the 256 KiB modelled
 * by tb_hazard3.sv. Freestanding: no newlib, no crt0 from the toolchain. */

OUTPUT_ARCH(riscv)
ENTRY(_reset)

MEMORY {
	RAM (rwx) : ORIGIN = 0x80000000, LENGTH = 256K
}

SECTIONS {
	. = 0x80000000;

	.vectors : { KEEP(*(.vectors)) } > RAM

	.text : {
		*(.text.start)
		*(.text .text.*)
	} > RAM

	.rodata : {
		*(.rodata .rodata.*)
		*(.srodata .srodata.*)
	} > RAM

	. = ALIGN(4);
	__DATA_BEGIN__ = .;
	.data : {
		*(.data .data.*)
	} > RAM
	.sdata : {
		__SDATA_BEGIN__ = .;
		*(.sdata .sdata.*)
	} > RAM

	. = ALIGN(4);
	__bss_start = .;
	.sbss : {
		*(.sbss .sbss.*)
		*(.scommon)
	} > RAM
	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	} > RAM
	. = ALIGN(4);
	__bss_end = .;

	/* gp covers .sdata/.sbss for single-instruction (relaxed) access. */
	__global_pointer$ = MIN(__SDATA_BEGIN__ + 0x800,
	                        MAX(__DATA_BEGIN__ + 0x800, __bss_end - 0x800));

	. = ALIGN(16);
	__stack_top = ORIGIN(RAM) + LENGTH(RAM);
}
